******************************************
RainMaker AT Commands and Firmware
******************************************

.. toctree::
   :maxdepth: 1
   
   RainMaker AT Command Set <RainMaker_AT_Command_Set>
   RainMaker AT Examples <RainMaker_AT_Examples>

.. _rm-at-messages:

RainMaker AT Messages
======================

.. _rm-at-messages-report:

- ESP-AT RainMaker Message Reports (active)

  ESP-AT will report important RainMaker state changes or messages in the system.

  .. list-table:: ESP-AT Message Reports
     :header-rows: 1
     :widths: 60 60

     * - ESP-AT RainMaker Message Report
       - Description
     * - Universal AT messages
       - Please refer to :ref:`ESP-AT Message Reports (active) <at-messages-report>`
     * - +RMRECV:<src_name>,<device_name>,<param_name>:<param_value>
       - ESP-AT received messages from the RainMaker cloud, ``<param_value>`` is a string
     * - +RMCONNECTED
       - RainMaker device connected to the cloud
     * - +RMDISCONNECTED
       - RainMaker device passively disconnected from the cloud
     * - +RMRESET
       - RainMaker device received reset message
     * - +RMREBOOT
       - RainMaker device received reboot message
     * - +RMTIMEZONE
       - RainMaker device received timezone message
     * - +RMMAPPINGDONE
       - RainMaker device completed user-node mapping
